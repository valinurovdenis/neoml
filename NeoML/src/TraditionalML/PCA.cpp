/* Copyright � 2017-2020 ABBYY Production LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
--------------------------------------------------------------------------------------------------------------*/

#include <common.h>
#pragma hdrstop

#include <NeoML/Dnn/DnnBlob.h>
#include <NeoML/TraditionalML/PCA.h>
#include <memory>

namespace NeoML {

CPCA::CPCA( int _components ): components( _components ), noiseVariance( 0 )
{
	NeoAssert( components >= 0 );
};

static CPtr<CDnnBlob> convertToBlob( IMathEngine& mathEngine, const CFloatMatrixDesc& data )
{
	const int vectorCount = data.Height;
	const int featureCount = data.Width;
	CPtr<CDnnBlob> result = CDnnBlob::CreateDataBlob( mathEngine, CT_Float, 1, vectorCount, featureCount );
	CFloatHandle currData = result->GetData();
	if( data.Columns == nullptr ) {
		mathEngine.DataExchangeTyped( currData, data.Values, vectorCount * featureCount );
	} else {
		CArray<float> row;
		for( int r = 0; r < vectorCount; ++r ) {
			row.Empty();
			row.Add( 0, featureCount );
			for( int i = data.PointerB[r]; i < data.PointerE[r]; i++ ) {
				row[data.Columns[i]] = data.Values[i];
			}
			mathEngine.DataExchangeTyped( currData, row.GetPtr(), featureCount );
			currData += featureCount;
		}
	}
	return result;
}

static void subtractMean( IMathEngine& mathEngine, const CFloatHandle& data, int matrixHeight, int matrixWidth )
{
	CPtr<CDnnBlob> meanVector = CDnnBlob::CreateVector( mathEngine, CT_Float, matrixWidth );
	CPtr<CDnnBlob> height = CDnnBlob::CreateVector( mathEngine, CT_Float, 1 );
	CFloatHandle& heightHandle = height->GetData();
	heightHandle.SetValue( -1. / matrixHeight );
	mathEngine.SumMatrixRows( 1, meanVector->GetData(), data, matrixHeight, matrixWidth );
	mathEngine.VectorMultiply( meanVector->GetData(), meanVector->GetData(), matrixWidth, heightHandle );
	mathEngine.AddVectorToMatrixRows( 1, data, data, matrixHeight, matrixWidth, meanVector->GetData() );
}

static void flipSVD( IMathEngine& mathEngine, const CFloatHandle& u, const CFloatHandle& vt, int m, int k, int n )
{
	CPtr<CDnnBlob> abs = CDnnBlob::CreateVector( mathEngine, CT_Float, m * k );
	CPtr<CDnnBlob> maxValues = CDnnBlob::CreateVector( mathEngine, CT_Float, k );
	CPtr<CDnnBlob> maxIndices = CDnnBlob::CreateVector( mathEngine, CT_Int, k );

	mathEngine.VectorAbs( u, abs->GetData(), m * k );
	mathEngine.FindMaxValueInColumns( 1, abs->GetData(), m, k, maxValues->GetData(), maxIndices->GetData<int>(), k );
	int* indices = maxIndices->GetBuffer<int>( 0, k, false );
	float* signs = maxValues->GetBuffer<float>( 0, k, false );
	for( int i = 0; i < k; i++ ) {
		signs[i] = ( u.GetValueAt( indices[i] * k + i ) >= 0 ? 1 : -1 );
	}
	maxValues->ReleaseBuffer( signs, false );
	mathEngine.MultiplyMatrixByDiagMatrix( u, m, k, maxValues->GetData<float>(), u, m * k );
	mathEngine.MultiplyDiagMatrixByMatrix( maxValues->GetData<float>(), k, vt, n, vt, n * k );
}

static void blobToMatrix( const CPtr<CDnnBlob>& blob, CSparseFloatMatrix& matrix, int matrixHeight, int matrixWidth, int blobWidth )
{
	CFloatVectorDesc row;
	row.Size = matrixWidth;

	int pos = 0;
	for( int i = 0; i < matrixHeight; i++, pos += blobWidth ) {
		row.Values = blob->GetBuffer<float>( pos, matrixWidth, false );
		matrix.AddRow( row );
	}
}

void CPCA::calculateVariance( IMathEngine& mathEngine, const CFloatHandle& s, int m, int k, int n )
{
	CPtr<CDnnBlob> var = CDnnBlob::CreateVector( mathEngine, CT_Float, k );
	CPtr<CDnnBlob> totalVar = CDnnBlob::CreateVector( mathEngine, CT_Float, 1 );
	CPtr<CDnnBlob> temp = CDnnBlob::CreateVector( mathEngine, CT_Float, 1 );
	CFloatHandle& tempHandle = temp->GetData();

	// calculate explained_variance
	mathEngine.VectorEltwiseMultiply( s, s, var->GetData(), k );
	tempHandle.SetValue( 1. / ( m - 1 ) );
	mathEngine.VectorMultiply( var->GetData(), var->GetData(), k, tempHandle );
	explainedVariance.SetSize( components );
	var->CopyTo( explainedVariance.GetPtr(), components );

	// calculate noise_variance
	if( components < k ) {
		mathEngine.VectorSum( var->GetData() + components, k - components, tempHandle );
		noiseVariance = tempHandle.GetValue() / ( k - components );
	} else {
		noiseVariance = 0;
	}

	// calculate explained_variance_ratio
	mathEngine.VectorSum( var->GetData(), k, totalVar->GetData() );
	tempHandle.SetValue( 1. / totalVar->GetData().GetValue() );
	mathEngine.VectorMultiply( var->GetData(), var->GetData(), k, tempHandle );
	explainedVarianceRatio.SetSize( components );
	var->CopyTo( explainedVarianceRatio.GetPtr(), components );
}

void CPCA::train( const CFloatMatrixDesc& data, bool isTransform )
{
	int n = data.Width;
	int m = data.Height;
	int k = min( n, m );
	NeoAssert( components <= k );

	std::unique_ptr<IMathEngine> mathEngine( CreateCpuMathEngine( 1, 0 ) );
	CPtr<CDnnBlob> a = convertToBlob( *mathEngine, data );
	CPtr<CDnnBlob> u = CDnnBlob::CreateDataBlob( *mathEngine, CT_Float, 1, m, k );
	CPtr<CDnnBlob> s = CDnnBlob::CreateVector( *mathEngine, CT_Float, k );
	CPtr<CDnnBlob> vt = CDnnBlob::CreateDataBlob( *mathEngine, CT_Float, 1, k, n );
	CPtr<CDnnBlob> superb = CDnnBlob::CreateVector( *mathEngine, CT_Float, k - 1 );

	// data -= mean(data)
	subtractMean( *mathEngine, a->GetData(), m, n );

	mathEngine->SingularValueDecomposition( a->GetData(), n, m, u->GetData(), s->GetData(), vt->GetData(), superb->GetData() );
	singularValues.SetSize( components );
	s->CopyTo( singularValues.GetPtr(), components );

	// flip signs of u columns and vt rows to obtain deterministic result
	flipSVD( *mathEngine, u->GetData(), vt->GetData(), m, k, n );

	calculateVariance( *mathEngine, s->GetData(), m, k, n );

	componentsMatrix = CSparseFloatMatrix( components, n, components * n );
	blobToMatrix( vt, componentsMatrix, components, n, n );

	if( isTransform ) {
		transformedMatrix = CSparseFloatMatrix( components, m, components * m );
		mathEngine->MultiplyMatrixByDiagMatrix( u->GetData(), m, k, s->GetData(), u->GetData(), m * k );
		blobToMatrix( u, transformedMatrix, m, components, k );
	}
}

void CPCA::Train( const CFloatMatrixDesc& data )
{
	train( data, false );
}

CSparseFloatMatrixDesc CPCA::Transform( const CFloatMatrixDesc& data )
{
	train( data, true );
	return transformedMatrix.GetDesc();
}

} // namespace NeoML